// Host-independent checks for device recovery, celestial stabilization, and
// the row-vector matrix conventions used by tracked light and temporal passes.
#include "../src/Core/DeviceHealth.h"
#include "../src/D3D9/CelestialStability.h"
#include "../src/D3D9/CameraContinuity.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>

using namespace renderer;

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
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }
    bool Contains(const std::string& text, const char* needle)
    {
        return text.find(needle) != std::string::npos;
    }
}

int main() try
{
    // D3D9 codes, as declared in the SDK's d3d9.h (MAKE_D3DHRESULT(2152/2153)).
    Require(kD3DErrDeviceLost == static_cast<long>(0x88760868u), "device-lost code matches SDK");
    Require(kD3DErrDeviceNotReset == static_cast<long>(0x88760869u), "device-not-reset code matches SDK");

    Require(ClassifyDeviceResult(0) == DeviceAvailability::Operational, "S_OK keeps effects running");
    Require(ClassifyDeviceResult(kD3DErrDeviceLost) == DeviceAvailability::Lost, "device lost pauses effects");
    Require(ClassifyDeviceResult(kD3DErrDeviceNotReset) == DeviceAvailability::Lost, "device awaiting reset pauses effects");
    Require(ClassifyDeviceResult(static_cast<long>(0x8876086Cu)) == DeviceAvailability::Operational,
            "invalid call is not treated as device loss");
    Require(ClassifyDeviceResult(static_cast<long>(0x8876017Cu)) == DeviceAvailability::Operational,
            "out-of-video-memory is not treated as device loss");

    {
        FrameGapDetector gaps;
        Require(gaps.Observe(10000, 1000), "first frame counts as a break");
        Require(!gaps.Observe(10016, 1000), "normal 60 Hz cadence is continuous");
        Require(!gaps.Observe(11016, 1000), "gap equal to the limit is still continuous");
        Require(gaps.Observe(13000, 1000), "Alt+Tab-length gap is a break");
        Require(!gaps.Observe(13016, 1000), "continuity resumes after a break");
        Require(gaps.Observe(100, 1000), "clock moving backwards is a break");
    }

    {
        RetryGate gate;
        Require(gate.Open(0), "retry gate starts open");
        Require(gate.Fail(1000, 2000), "first failure is reported");
        Require(!gate.Open(1500), "failed pass waits out the cooldown");
        Require(!gate.Fail(1500, 2000), "repeated failure is not reported as new");
        Require(gate.Open(3500), "pass is retried after the cooldown");
        gate.Succeed();
        Require(!gate.Failed() && gate.Open(3501), "success clears the failure streak");
        Require(gate.Fail(4000, 2000), "a new streak after recovery is reported again");
    }

    {
        // Click-an-NPC brightening: the camera whose light the frame inherits
        // is latched off the first qualifying draw, so a portrait-style draw
        // must not be able to become it. The gate sees the same rows the
        // capture checks (v[0..3]) plus the viewport size.
        auto viewFor = [](float px, float py, float pz, float yaw, float translation[4], float basis[3][4]) {
            const float c = std::cos(yaw), s = std::sin(yaw);
            const float rows[3][3] = {{c, -s, 0.f}, {s, c, 0.f}, {0.f, 0.f, 1.f}};
            for (int r = 0; r < 3; ++r) {
                for (int j = 0; j < 3; ++j) basis[r][j] = rows[r][j];
                basis[r][3] = 0.f;
                // Row-vector world->view translation: t = -cameraWorld * R (column j dot cameraWorld).
                translation[r] = -(rows[0][r] * px + rows[1][r] * py + rows[2][r] * pz);
            }
            translation[3] = 1.f;
        };
        float t[4], basis[3][4];
        CameraContinuityGate gate;
        viewFor(5100.f, -3200.f, 110.f, .4f, t, basis);
        Require(gate.Accept(t, basis, 1920, 1080), "the first full-frame camera anchors the gate");
        // A portrait camera: close-up, differently aimed, small viewport.
        viewFor(5102.f, -3198.f, 112.f, 2.6f, t, basis);
        Require(!gate.Accept(t, basis, 256, 256), "a portrait-viewport camera cannot take over the frame's light");
        // Even a full-frame interloper from somewhere else is not this camera.
        Require(!gate.Accept(t, basis, 1920, 1080), "a distant, differently aimed camera cannot take over either");
        // The real camera next frame: a few yards on at continental coordinates, barely turned.
        viewFor(5104.f, -3197.f, 110.5f, .47f, t, basis);
        Require(gate.Accept(t, basis, 1920, 1080), "the scene camera still latches after the interloper was refused");
        // A teleport: three consistent candidates re-anchor, no sooner.
        viewFor(800.f, -400.f, 30.f, .47f, t, basis);
        Require(!gate.Accept(t, basis, 1920, 1080), "a teleport is not believed on the first frame");
        Require(!gate.Accept(t, basis, 1920, 1080), "nor on the second");
        Require(gate.Accept(t, basis, 1920, 1080), "three consistent candidates re-anchor after a teleport");
        // Looking straight up at the sky: when terrain/WMO draws are culled,
        // the skybox D3DTS_VIEW basis tracks pitch up to the zenith while
        // preserving the anchored world camera position.
        for (float pitch : {.3f, .8f, 1.3f, 1.56f}) {
            const float cy = std::cos(.47f), sy = std::sin(.47f);
            const float cp = std::cos(pitch), sp = std::sin(pitch);
            float skyBasis[3][4] = {
                {cy, -sy * cp, sy * sp, 0.f},
                {sy,  cy * cp, -cy * sp, 0.f},
                {0.f, sp,       cp,      0.f}};
            Require(gate.MatchesBasis(skyBasis, 1920, 1080), "upward-pitching sky basis stays continuous with the scene camera");
            gate.UpdateBasis(skyBasis, 1920, 1080);
            float skyTranslation[4]{};
            const float anchor[3]={800.f,-400.f,30.f};
            for(int c=0;c<3;++c)
                skyTranslation[c]=-(anchor[0]*skyBasis[0][c]+anchor[1]*skyBasis[1][c]+anchor[2]*skyBasis[2][c]);
            skyTranslation[3]=1.f;
            Require(gate.AcceptFallback(skyTranslation,skyBasis,1920,1080),
                    "sky fallback accepts a hard camera pitch at the continuous world position");
            Require(std::fabs(gate.Position()[0] - 800.f) < 1e-3f &&
                    std::fabs(gate.Position()[1] + 400.f) < 1e-3f &&
                    std::fabs(gate.Position()[2] - 30.f) < 1e-3f,
                "updating the sky basis preserves the anchored world camera position");
        }
        viewFor(850.f,-400.f,30.f,.47f,t,basis);
        Require(!gate.AcceptFallback(t,basis,1920,1080),
                "sky fallback still rejects a different world camera at the same viewport");
        Require(!gate.AcceptFallback(t,basis,256,256),
                "sky fallback rejects an unrelated viewport even when its camera is close");
        gate.Reset();
        viewFor(0.f, 0.f, 0.f, 0.f, t, basis);
        Require(gate.Accept(t, basis, 1920, 1080), "a reset gate anchors on the next camera it sees");
    }

    {
        // Moon lighting following the camera: a view-space disc direction is
        // only a world direction once the frame's real camera converts it.
        // For a fixed moon, every camera yaw must recover the same world
        // vector - build the view direction the way TransformPointRowVector
        // and the shaders themselves do (pos_view = pos_world * V, i.e.
        // column dots) and convert it back through viewRaw (row dots, V^T).
        const Vec3 moonWorld = Normalize({.35f, -.62f, .70f});
        for (float yaw : {0.f, .7f, 1.9f, 3.4f, 5.8f}) {
            const float c = std::cos(yaw), s = std::sin(yaw);
            float viewRaw[16] = {c, -s, 0.f, 0.f, s, c, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f};
            const float viewDir[3] = {
                viewRaw[0] * moonWorld.x + viewRaw[4] * moonWorld.y + viewRaw[8] * moonWorld.z,
                viewRaw[1] * moonWorld.x + viewRaw[5] * moonWorld.y + viewRaw[9] * moonWorld.z,
                viewRaw[2] * moonWorld.x + viewRaw[6] * moonWorld.y + viewRaw[10] * moonWorld.z};
            float world[3];
            ViewDirectionToWorld(viewRaw, viewDir, world);
            const float dot = world[0] * moonWorld.x + world[1] * moonWorld.y + world[2] * moonWorld.z;
            Require(dot > .9999f, "the moon's world direction is the same from every camera yaw");
        }
        const auto composite = Read("VolumeIntegration.h");
        Require(Contains(composite, "renderer::ViewDirectionToWorld(&frameCtx.viewRaw.m[0][0]") &&
                Contains(composite, "moon.visible ? &moonDirectionWorld : nullptr") &&
                !Contains(composite, "moon.visible ? &moon.worldSpaceDirection : nullptr"),
            "the compositor converts celestial directions with the captured camera, not the billboard draw's");
        const auto capture = Read("src/D3D9/CameraCapture.cpp");
        Require(Contains(capture, "if (!fullFrame)") && Contains(capture, "m_cameraGate.Accept(v[3], v, vp.Width, vp.Height)"),
            "camera capture demands a full-frame, continuous camera before latching the frame's light");
        Require(Contains(capture, "CameraCapture::ObserveSkyFallback") &&
                Contains(capture, "CameraCapture::ApplySkyFallback") &&
                Contains(composite, "CaptureSkyFallback()"),
            "sky fallback camera capture keeps clouds alive when looking straight up and terrain is culled");

        // ORGRIMMAR FROM STREET LEVEL: THE CLOUD DECK AND THE LAMPS FOLLOWING THE CAMERA.
        // Where no draw ever carries a full-frame rigid camera (a city seen from the street,
        // with everything overhead culled), the fallback used to take the FIRST transform of
        // the frame as the camera. A leftover sky/moon/identity transform then became the pose:
        // the deck and the lights were anchored to its translation and swung with the mouse.
        // Pinned here: candidate selection, the screen convention that lets the frame's own disc
        // testify, jump confirmation, and the stale-pose guards on every world-anchored pass.
        auto yawBasis = [](float yaw, float b[3][4]) {
            const float c = std::cos(yaw), s = std::sin(yaw);
            for (int r = 0; r < 3; ++r) for (int k = 0; k < 4; ++k) b[r][k] = 0.f;
            b[0][0] = c; b[0][1] = -s; b[1][0] = s; b[1][1] = c; b[2][2] = 1.f;
        };
        float identityBasis[3][4]{};
        identityBasis[0][0] = identityBasis[1][1] = identityBasis[2][2] = 1.f;

        {
            // The projection must use COLUMN dots (pos_view = pos_world * V), the
            // same convention as CelestialTracker's own screen mapping. Row dots
            // would run the inverse rotation and mirror the disc for every yaw.
            const float world[3] = { -3.f, 0.f, 4.f };
            float x0 = 0.f, y0 = 0.f, x1 = 0.f, y1 = 0.f;
            float turned[3][4];
            yawBasis(1.57079632679f, turned);
            Require(ProjectWorldDirection(identityBasis, 1.f, 1.f, world, x0, y0) &&
                    ProjectWorldDirection(turned, 1.f, 1.f, world, x1, y1),
                "a disc in front of the camera projects to a screen position");
            Require(x0 < .5f && std::fabs(y0 - .5f) < 1e-4f,
                "a body left of the view axis projects left of centre");
            Require(y1 < .5f && std::fabs(x1 - .5f) < 1e-4f,
                "a quarter turn moves the body to the top of the screen instead of mirroring it");

            // A pure yaw over one world axis still has to register: the row of
            // the turned axis stays put, the other two carry the rotation.
            const float agreement = BasisAgreement(identityBasis, turned);
            Require(std::fabs(agreement) < 1e-4f,
                "a quarter turn is not mistaken for a continuous view");
            float small[3][4];
            yawBasis(.05f, small);
            Require(BasisAgreement(identityBasis, small) > .99f,
                "a normal mouse-look frame still agrees with the previous camera");
        }

        {
            // The frame's own painted sun/moon disc is the evidence: the basis
            // that reproduces where the client put the disc is the camera.
            const float world[3] = { -3.f, 0.f, 4.f };
            DiscObservation disc;
            disc.available = true;
            disc.projectionX = 1.f;
            disc.projectionY = 1.f;
            disc.world[0] = world[0]; disc.world[1] = world[1]; disc.world[2] = world[2];
            ProjectWorldDirection(identityBasis, 1.f, 1.f, disc.world, disc.screenX, disc.screenY);

            float wrong[3][4];
            yawBasis(1.57079632679f, wrong);
            Require(ContradictsDisc(wrong, disc),
                "a transform that puts the painted disc somewhere else is rejected");
            Require(!ContradictsDisc(identityBasis, disc),
                "the camera that painted the disc agrees with it");

            SkyPoseSelector selector;
            selector.SetReference(identityBasis);
            float chosen[3][4];
            bool corroborated = true;
            selector.BeginFrame();
            selector.Observe(wrong);
            Require(!selector.Resolve(disc, chosen, corroborated) && !corroborated,
                "a sky transform that contradicts the painted disc never becomes the camera");

            selector.BeginFrame();
            selector.Observe(wrong);
            selector.Observe(identityBasis);
            Require(selector.Resolve(disc, chosen, corroborated) && corroborated,
                "the candidate the disc corroborates is accepted in the same frame");
            Require(BasisAgreement(chosen, identityBasis) > .9999f,
                "the disc-consistent candidate is the one published");

            SkyPoseSelector empty;
            empty.SetReference(identityBasis);
            empty.BeginFrame();
            Require(!empty.Resolve(DiscObservation{}, chosen, corroborated),
                "a frame that offers no transform publishes no pose instead of inventing one");
        }

        {
            // No disc on screen (neutral pitch under the Orgrimmar sky): only
            // continuity with the accepted camera may corroborate. A jump is
            // held one frame, and a transform that changes every frame is never
            // adopted, so a stray full-screen transform cannot take the frame.
            SkyPoseSelector selector;
            selector.SetReference(identityBasis);
            float jumped[3][4];
            yawBasis(1.0f, jumped);
            const DiscObservation none;
            float chosen[3][4];
            bool corroborated = true;

            selector.BeginFrame();
            selector.Observe(jumped);
            Require(!selector.Resolve(none, chosen, corroborated) && !corroborated,
                "a discontinuous pose is not believed on its first frame");
            selector.BeginFrame();
            selector.Observe(jumped);
            Require(selector.Resolve(none, chosen, corroborated) &&
                    BasisAgreement(chosen, jumped) > .9999f,
                "the same discontinuous pose twice is confirmed on the second frame");

            for (int i = 0; i < 3; ++i)
            {
                float stray[3][4];
                yawBasis(3.0f + .9f * float(i), stray);
                selector.BeginFrame();
                selector.Observe(stray);
                Require(!selector.Resolve(none, chosen, corroborated),
                    "a transform that changes every frame is never adopted");
            }
        }

        // The guards themselves. Every pass that places something in world
        // space from this pose must refuse a stale one - the deck, the lamp
        // pool, the torch emitters and the shafts each reconstruct positions
        // from the frame camera, and from a held pose the content follows the
        // player instead of the world.
        {
            const auto clouds = Read("src/Effects/VolumetricClouds.cpp");
            const auto localLighting = Read("src/Effects/LocalLightingRenderer.cpp");
            const auto torches = Read("src/Lighting/LocalLightManager.cpp");
            const auto frameReset = Read("src/Core/FrameContext.cpp");
            Require(Contains(clouds, "if(f.cameraPoseStale)return false;"),
                "the cloud deck refuses to draw from a stale camera pose");
            Require(Contains(localLighting, "f.cameraPoseStale") &&
                    Contains(localLighting, "FrameContext::kCameraEyeHoldMs"),
                "local lighting refuses a stale pose or an eye no draw carried");
            Require(Contains(torches, "f.cameraPoseStale") &&
                    Contains(torches, "FrameContext::kCameraEyeHoldMs"),
                "torch emitters are not placed through a stale pose");
            Require(Contains(frameReset, "cameraPoseStale = true;"),
                "a new frame starts with the previous pose marked unusable");
        }

        {
            const auto capture = Read("src/D3D9/CameraCapture.cpp");
            const auto composite = Read("VolumeIntegration.h");
            Require(Contains(capture, "m_skyPose.Observe(basis)") &&
                    !Contains(capture, "m_fallbackCandidateValid"),
                "the sky fallback collects candidates instead of latching the first transform");
            Require(Contains(capture, "fabsf(lengthSquared - 1.0f) > 0.01f"),
                "a scaled or skewed sky transform is not a rigid camera basis");
            Require(Contains(capture, "const float* anchor = m_cameraGate.Position();") &&
                    Contains(capture, "cameraPoseSource = FrameContext::PoseHeld"),
                "the sky fallback anchors the eye to the accepted camera position only");
            Require(Contains(capture, "frameContext.cameraPoseStale = false;"),
                "a real capture publishes a fresh pose");
            Require(Contains(composite, "cameraCaptureShaderHash = 0;") &&
                    Contains(composite, "capturedViewValid = false;"),
                "capture evidence is cleared every frame, so a failed attempt cannot linger");
            const auto attempt = composite.find("ready = CaptureCamera(d);");
            const auto publish = composite.find("cameraCaptureShaderHash = renderer::g_trackedState.vsHash;");
            Require(attempt != std::string::npos && publish != std::string::npos && attempt < publish,
                "the terrain shader is published only after the capture that produced it succeeded");
        }

        // MULTISAMPLING, AND THE ANTI-ALIASING THAT REPLACES IT. A multisampled depth buffer cannot be
        // sampled by a pixel shader in D3D9 at all, so above 1x every effect here that reads depth
        // loses it - water refraction, absorption and shore foam, ambient occlusion, local lighting,
        // rain roof occlusion, fog - and those do not degrade, they disappear. Two guards that followed
        // from that were wrong in opposite directions. The colour capture refused multisampling even
        // though copying a multisampled target into a plain render-target texture IS the resolve, so
        // reflections and refraction threw their colour source away for nothing. And the image pass had
        // no anti-aliasing of any kind, so a player who did the right thing and ran at 1x had no way to
        // get their edges back. Pinned: the colour guard is gone, the MRT guard stays (that one is a
        // real limit - D3D9 makes every bound target share one multisample type), FXAA is in the image
        // pass at the sub-pixel tap offset that is the difference between it working and it compiling,
        // it runs on its own switch, and the grade is forced to identity when the grade master is off.
        const auto imagePass = Read("src/Effects/ImagePostProcess.h");
        const auto effects = Read("VolumeEffects.h");
        const auto reflection = Read("WaterReflection.h");
        const auto water = Read("WaterEffect.h");
        const auto overlay = Read("src/Environment/OverlayItems.h");
        const auto ini = Read("GraphicsEffects.ini");
        Require(!Contains(reflection, "desc.MultiSampleType!=D3DMULTISAMPLE_NONE") &&
                Contains(reflection, "if(FAILED(volume::target->GetDesc(&desc))){++failedFrames;return;}"),
            "the SSR colour capture resolves a multisampled target instead of refusing it");
        Require(Contains(water, "if(colorDesc.MultiSampleType!=D3DMULTISAMPLE_NONE)return;"),
            "the water MRT stays guarded, because every bound target must share one multisample type");
        Require(Contains(effects, "float4 aaParams:register(c2);") &&
                Contains(effects, "uv+float2(-texel.x,-texel.y)*.5") &&
                Contains(effects, "uv+float2(texel.x,texel.y)*.5") &&
                !Contains(effects, "float3 nw=tex2D(scene,uv+float2(-texel.x,-texel.y)).rgb;") &&
                Contains(effects, "if(lumaMax-lumaMin<max(.0312,lumaMax*.125))return centre;") &&
                Contains(effects, "col=saturate(col+lap*(postParams.w*.75)*(1.0-aaEdge));"),
            "FXAA taps the corners pixels share for a sub-pixel edge, skips flat areas, and does not sharpen what it just smoothed");
        Require(Contains(imagePass, "const float* parameters,const float* aaParameters") &&
                Contains(imagePass, "SetPixelShaderConstantF(2,aaParameters,1)"),
            "the anti-aliasing switch is always uploaded, so it cannot inherit a stale constant register");
        Require(Contains(composite, "(gradeWanted || antiAliasingEnabled)") &&
                Contains(composite, "gradeWanted?brightnessPercent*.01f:0.f") &&
                Contains(composite, "antiAliasingEnabled = ReadTuning(L\"AntiAliasing\", 1, L\"PostProcess\") != 0;"),
            "anti-aliasing runs on its own switch and forces an identity grade when the master is off");
        Require(Contains(overlay, "L\"PostProcess\",L\"AntiAliasing\"") && Contains(ini, "AntiAliasing=1"),
            "anti-aliasing is in the F7 image tab and in the shipped INI, both defaulting on");
    }

    {
        // Strength/body selection has no screen-space inputs. Day/night
        // transitions still work, including a bright, cool-tinted noon.
        CelestialLightStabilizer light;
        CelestialLightInput frame;
        frame.deltaSeconds = 1.0f / 60.0f;
        frame.daylight = 0.8f;
        Require(light.Update(frame).intensity == 1.0f, "daylight primes direct light without a visible disc");
        for (int i = 0; i < 600; ++i) light.Update(frame);
        Require(light.Update(frame).intensity == 1.0f, "direct light does not decay while daylight remains");
        frame.daylight = 0.0f;
        const float fading = light.Update(frame).intensity;
        Require(fading > 0.0f && fading < 1.0f, "sunset fades direct light instead of stepping it off");
        for (int i = 0; i < 600; ++i) light.Update(frame);
        Require(light.Update(frame).intensity == 0.0f, "direct light is off once darkness has settled");
        frame.moonlight = 0.65f;
        frame.directionalLuminance = 0.3f;
        for (int i = 0; i < 600; ++i) light.Update(frame);
        auto night = light.Update(frame);
        Require(night.intensity == 1.0f && night.isMoon, "moonlight period selects the moon for direct light");
        frame.moonlight = 0.9f;
        frame.directionalLuminance = 1.0f;
        Require(!light.Update(frame).isMoon, "bright cool-tinted noon selects sunlight regardless of past discs");
        frame.moonlight = 0.0f;
        Require(light.Update(frame).intensity == 1.0f, "bright untinted light survives zero day/night factors");
        frame.directionalLuminance = 0.3f;
        frame.daylight = 0.3f;
        frame.moonlight = 0.4f;
        Require(!light.Update(frame).isMoon, "twilight hysteresis retains sunlight inside the band");
        frame.moonlight = 0.8f;
        Require(light.Update(frame).isMoon, "a real moonlight period takes over from sunlight");
        frame.daylight = 0.9f;
        frame.moonlight = 0.1f;
        Require(!light.Update(frame).isMoon, "dawn restores sunlight without needing a visible sun");
        light.Reset();
        frame = {};
        Require(!light.Primed() && light.Update(frame).intensity == 0.0f, "reset clears direct-light history");
    }

    {
        // Exercise the compositor's separate visible-disc and world-light
        // paths. Previously a daytime moon became the dominant world light
        // whenever the camera culled the sun (and vice versa at night).
        const Vec3 sun = Normalize({.6f, .2f, .8f});
        const Vec3 moon = Normalize({-.8f, -.2f, .01f});
        for (bool night : {false, true})
        {
            CelestialLightStabilizer light;
            CelestialDirectionStabilizer direction;
            direction.Update(&sun, false, .016f);
            direction.Update(&moon, true, .016f);
            CelestialLightInput scene;
            scene.daylight = night ? 0.f : .8f;
            scene.moonlight = night ? .8f : 0.f;
            scene.directionalLuminance = night ? .3f : .8f;
            const Vec3 expected = night ? moon : sun;
            for (unsigned visible = 0; visible < 4; ++visible)
            {
                // Long enough to catch delayed/held changes, not just a
                // one-frame blink. Both, either, or neither disc can be seen.
                for (int i = 0; i < 2400; ++i)
                {
                    const bool sunVisible = (visible & 1) != 0;
                    const bool moonVisible = (visible & 2) != 0;
                    const bool glareIsMoon = moonVisible && (night || !sunVisible);
                    const auto stable = light.Update(scene);
                    const auto sunWorld = direction.Update(sunVisible ? &sun : nullptr, false, scene.deltaSeconds);
                    const auto moonWorld = direction.Update(moonVisible ? &moon : nullptr, true, scene.deltaSeconds);
                    const auto world = stable.isMoon ? moonWorld : sunWorld;
                    if (stable.isMoon != night || stable.intensity != 1.f || Length(world - expected) > 1e-5f ||
                        std::fabs(CloudShadowElevationFade(world.z) - CloudShadowElevationFade(expected.z)) > 1e-5f)
                        throw std::runtime_error("visible glare body changed world cloud shadows");
                    if (visible == 2 && !night && !glareIsMoon)
                        throw std::runtime_error("test must exercise a daytime moon glare origin");
                }
            }
        }
        Require(true, "sun/moon/both/neither visible cannot switch world-light kind, direction or shadow strength");
    }

    {
        // CelestialTracker must undo the row-vector VIEW rotation, not apply
        // it a second time. A fixed world-space moon should produce different
        // view directions as the camera yaws, but always recover the same
        // world direction (the old column/row mix made the light rotate with
        // the camera and also drove cloud-shadow elevation toward zero).
        const Vec3 worldDirection = Normalize({.31f, -.47f, .82f});
        constexpr float yawPitchDegrees[][2] = {
            {-105.f, 0.f}, {-45.f, 25.f}, {0.f, 0.f}, {35.f, -40.f}, {90.f, 65.f}};
        bool invariant = true;
        for (const auto& angles : yawPitchDegrees)
        {
            const float yaw = angles[0] * 3.14159265358979f / 180.f;
            const float pitch = angles[1] * 3.14159265358979f / 180.f;
            const float cy = std::cos(yaw), sy = std::sin(yaw);
            const float cp = std::cos(pitch), sp = std::sin(pitch);
            Matrix4 yawView, pitchView;
            yawView.m[0][0] = cy;  yawView.m[0][1] = sy;
            yawView.m[1][0] = -sy; yawView.m[1][1] = cy;
            pitchView.m[1][1] = cp;  pitchView.m[1][2] = sp;
            pitchView.m[2][1] = -sp; pitchView.m[2][2] = cp;
            const Matrix4 view = Matrix4::Multiply(yawView, pitchView);
            const Vec3 viewDirection = TransformDirectionRowVector(worldDirection, view.m);
            const Vec3 recovered = ViewDirectionToWorld(viewDirection, view.m);
            invariant = invariant && Dot(recovered, worldDirection) >= .99999f;
        }
        Require(invariant, "confirmed celestial world direction is invariant under camera yaw and pitch");

        // The temporal shader's previous-frame position transform must use
        // these same row-vector columns plus row-three translation. A 90
        // degree yaw and non-zero camera translation make a row/column swap
        // obvious rather than accidentally passing under an identity view.
        Matrix4 translatedView;
        translatedView.m[0][0] = 0.f; translatedView.m[0][1] = 1.f;
        translatedView.m[1][0] = -1.f; translatedView.m[1][1] = 0.f;
        translatedView.m[3][0] = -10.f; translatedView.m[3][1] = 5.f;
        translatedView.m[3][2] = -2.f;
        const Vec3 viewPoint = TransformPointRowVector({2.f, 3.f, 4.f}, translatedView.m);
        Require(std::fabs(viewPoint.x + 13.f) < 1e-5f &&
                std::fabs(viewPoint.y - 7.f) < 1e-5f &&
                std::fabs(viewPoint.z - 2.f) < 1e-5f,
                "row-vector view transform uses matrix columns and translation");
    }

    {
        CelestialDirectionStabilizer direction;
        const Vec3 sun = Normalize({.6f, .2f, .8f});
        auto stable = direction.Update(&sun, false, 1.f/60.f);
        Require(Length(stable-sun)<1e-6f, "confirmed world direction primes the sun");
        for (int i=0;i<600;++i)
        {
            stable=direction.Update(nullptr, false, 1.f/60.f);
            if (Length(stable-sun)>=1e-6f) throw std::runtime_error("camera fallback moved sunlight");
        }
        Require(Length(stable-sun)<1e-6f, "camera-dependent fallback cannot move confirmed sunlight");
        const Vec3 moon=Normalize({-.2f,-.6f,.7f});
        Require(Length(direction.Update(&moon, true, .016f)-moon)<1e-6f,
                "moon keeps a separate confirmed direction");
        Require(Length(direction.Update(nullptr, false, .016f)-sun)<1e-6f,
                "returning to sunlight restores its own direction");
        const Vec3 moved=Normalize({.7f,.2f,.7f});
        stable=direction.Update(&moved, false, .016f);
        Require(Length(stable-sun)<Length(moved-sun) && std::fabs(Length(stable)-1.f)<1e-5f,
                "real celestial movement is smoothed and normalized");
        direction.Reset();
        Require(Length(direction.Update(nullptr, false, .016f)-Vec3{0,0,1})<1e-6f,
                "loading reset allows a fresh world direction");
    }

    {
        // Before confirmation, no camera-derived source may steer the
        // shadows. The old rate cap reached the horizon after 36 seconds.
        CelestialDirectionStabilizer direction;
        for (int i = 0; i < 7200; ++i)
        {
            for (bool moon : {false, true})
                if (Length(direction.Update(nullptr, moon, 1.f/60.f) - Vec3{0,0,1}) > 1e-6f)
                    throw std::runtime_error("unconfirmed world light drifted");
        }
        Require(true, "unconfirmed light stays world-fixed for minutes, not just one frame");
        const Vec3 zero{};
        Require(Length(direction.Update(&zero, false, .016f) - Vec3{0,0,1}) < 1e-6f,
                "invalid observation does not prime a body");
        const Vec3 actual = Normalize({.5f, -.2f, .8f});
        Require(Length(direction.Update(&actual, false, .016f) - actual) < 1e-6f,
                "first actual disc replaces the unconfirmed approximation");
        Require(Length(direction.Update(&zero, false, .016f) - actual) < 1e-6f,
                "invalid observation cannot replace a confirmed world direction");
        const Vec3 invalid{std::numeric_limits<float>::quiet_NaN(), 0, 1};
        Require(Length(direction.Update(&invalid, false, .016f) - actual) < 1e-6f,
                "non-finite observation cannot poison cloud shadows");
    }

    Require(CloudShadowElevationFade(0.0f) == 0.0f, "cloud shadows are off for a disc below the horizon");
    Require(CloudShadowElevationFade(0.03f) == 0.0f, "cloud shadows fade in from the horizon gate");
    Require(CloudShadowElevationFade(0.15f) == 1.0f, "cloud shadows reach full strength above the fade band");
    Require(CloudShadowElevationFade(0.09f) > 0.0f && CloudShadowElevationFade(0.09f) < 1.0f,
            "cloud shadow strength is continuous inside the fade band");
    Require(CloudShadowElevationFade(1.0f) == 1.0f, "an overhead disc casts full cloud shadows");

    Require(!HasWorldContext(false, false), "UI/loading frame with no world evidence is not sampleable");
    Require(HasWorldContext(false, true), "known terrain or liquid draws keep waves-only frames sampleable without INTZ");
    Require(HasWorldContext(true, false), "captured depth remains sufficient world evidence");
    Require(ShouldHoldLocation(HasWorldContext(false, false), true), "no-depth frame without terrain holds a confirmed location");
    Require(!ShouldHoldLocation(HasWorldContext(false, false), false), "no-depth frame with nothing confirmed still polls, so a first location can arrive");
    Require(!ShouldHoldLocation(HasWorldContext(false, true), true), "terrain-only world frame permits location sampling");

    // Cached D3DSBT_ALL blocks retain texture bindings, including DEFAULT-pool
    // render targets. The reset hook must invalidate this cache before it
    // forwards a real mode change to the game driver.
    const auto passState = Read("src/D3D9/ScopedPassState.h");
    const auto hooks = Read("ModernWoWRenderer.cpp");
    Require(Contains(passState, "inline void ResetPassStateBlocks() noexcept") &&
            Contains(passState, "for (auto& block : shared.block) block.Reset()") &&
            Contains(passState, "shared.deviceRef.Reset()"),
            "reset cleanup releases cached D3D state blocks and their device pin");
    const auto cleanup = hooks.find("void ResetEffectStateForDeviceChange(IDirect3DDevice9* d) noexcept");
    const auto originalReset = hooks.find("HRESULT WINAPI HookedReset", cleanup);
    Require(cleanup != std::string::npos && originalReset != std::string::npos &&
            hooks.find("renderer::detail::ResetPassStateBlocks();", cleanup) < originalReset,
            "state-block references are released before the game device reset");

    std::printf("ALL DEVICE HEALTH TESTS PASSED\n");
    return 0;
}
catch (const std::exception& error)
{
    std::printf("FAIL %s\n", error.what());
    return 1;
}
