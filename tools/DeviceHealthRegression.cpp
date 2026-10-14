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
            const float rows[3][3] = {{c, s, 0.f}, {-s, c, 0.f}, {0.f, 0.f, 1.f}};
            for (int r = 0; r < 3; ++r) {
                for (int j = 0; j < 3; ++j) basis[r][j] = rows[r][j];
                basis[r][3] = 0.f;
                translation[r] = -(rows[r][0] * px + rows[r][1] * py + rows[r][2] * pz);
            }
            translation[3] = 1.f;
        };
        float t[4], basis[3][4];
        CameraContinuityGate gate;
        viewFor(100.f, 200.f, 10.f, .4f, t, basis);
        Require(gate.Accept(t, basis, 1920, 1080), "the first full-frame camera anchors the gate");
        // A portrait camera: close-up, differently aimed, small viewport.
        viewFor(102.f, 198.f, 12.f, 2.6f, t, basis);
        Require(!gate.Accept(t, basis, 256, 256), "a portrait-viewport camera cannot take over the frame's light");
        // Even a full-frame interloper from somewhere else is not this camera.
        Require(!gate.Accept(t, basis, 1920, 1080), "a distant, differently aimed camera cannot take over either");
        // The real camera next frame: a few yards on, barely turned.
        viewFor(104.f, 203.f, 10.5f, .47f, t, basis);
        Require(gate.Accept(t, basis, 1920, 1080), "the scene camera still latches after the interloper was refused");
        // A teleport: three consistent candidates re-anchor, no sooner.
        viewFor(800.f, -400.f, 30.f, .47f, t, basis);
        Require(!gate.Accept(t, basis, 1920, 1080), "a teleport is not believed on the first frame");
        Require(!gate.Accept(t, basis, 1920, 1080), "nor on the second");
        Require(gate.Accept(t, basis, 1920, 1080), "three consistent candidates re-anchor after a teleport");
        gate.Reset();
        viewFor(0.f, 0.f, 0.f, 0.f, t, basis);
        Require(gate.Accept(t, basis, 1920, 1080), "a reset gate anchors on the next camera it sees");
    }

    {
        // Moon lighting following the camera: a view-space disc direction is
        // only a world direction once the frame's real camera converts it.
        // For a fixed moon, every camera yaw must recover the same world
        // vector - build the view direction the way the scene itself would
        // (vd is the world direction read against the camera basis rows) and
        // convert it back through viewRaw.
        const Vec3 moonWorld = Normalize({.35f, -.62f, .70f});
        for (float yaw : {0.f, .7f, 1.9f, 3.4f, 5.8f}) {
            const float c = std::cos(yaw), s = std::sin(yaw);
            float viewRaw[16] = {c, s, 0.f, 0.f, -s, c, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f};
            const float viewDir[3] = {
                viewRaw[0] * moonWorld.x + viewRaw[1] * moonWorld.y + viewRaw[2] * moonWorld.z,
                viewRaw[4] * moonWorld.x + viewRaw[5] * moonWorld.y + viewRaw[6] * moonWorld.z,
                viewRaw[8] * moonWorld.x + viewRaw[9] * moonWorld.y + viewRaw[10] * moonWorld.z};
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

    std::printf("ALL DEVICE HEALTH TESTS PASSED\n");
    return 0;
}
catch (const std::exception& error)
{
    std::printf("FAIL %s\n", error.what());
    return 1;
}
